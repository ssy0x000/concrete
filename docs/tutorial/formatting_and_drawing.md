# Formatting and Drawing

## Formatting

You can convert your compiled circuit into its textual representation by converting it to string:

<!--pytest-codeblocks:skip-->
```python
str(circuit)
```

If you just want to see the output on your terminal, you can directly print it as well:

<!--pytest-codeblocks:skip-->
```python
print(circuit)
```

{% hint style="warning" %}
Formatting is just for debugging purposes. It's not possible to create the circuit back from its textual representation. See [How to Deploy](../howto/deploy.md) if that's your goal.
{% endhint %}

## Drawing

{% hint style="danger" %}
Drawing functionality requires the installation of the package with the full feature set. See the [Installation](../getting-started/installing.md) section to learn how to do that.
{% endhint %}

You can use the `draw` method of your compiled circuit to draw it:

<!--pytest-codeblocks:skip-->
```python
drawing = circuit.draw()
```

This method will draw the circuit on a temporary PNG file and return the path to this file.

You can show the drawing in a Jupyter notebook, like this:

<!--pytest-codeblocks:skip-->
```python
from PIL import Image
drawing = Image.open(circuit.draw())
drawing.show()
drawing.close()
```

Or, you can use the `show` option of the `draw` method to show the drawing with `matplotlib`.

<!--pytest-codeblocks:skip-->
```python
circuit.draw(show=True)
```

{% hint style="danger" %}
Beware that this will clear the matplotlib plots you have.
{% endhint %}

Lastly, you can save the drawing to a specific path:

<!--pytest-codeblocks:skip-->
```python
destination = "/tmp/path/of/your/choice.png"
drawing = circuit.draw(save_to=destination)
assert drawing == destination
```
